# f7-unify: one GPU family harness for Savage, Radeon, Matrox, Mach64 and VMware

- **Written:** 2026-10-28 16:31 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Worktree `/home/peronslayer/Desktop/CiukiOS-wt-f7-unify`, branch `wt/f7-unify` = `wt/integ8`, which has:
  - main, with the security programme, sec-runtime1, fx-games20 and f7-26;
  - sec-k08 and sec-user-06;
  - the LAN round (f6-25/26/27);
  - f7-24 Radeon.

  The lead has started `git merge wt/f7-25` (Matrox); the conflicts are in the files `git diff --name-only --diff-filter=U` lists. After you, the lead merges f7-27 (Mach64) and f7-31 (VMware SVGA3D) into this branch, and each will be queued to you again with this directive.

## Scope

1. **One parametrization of the GL host harness.** Each family branch invented its own: Radeon `kernel_family='radeon'` with `-DGL_KERNEL_FAMILY_RADEON`, Matrox `family=`, `backend=` and `kernel_engine=`. Make a single family table in `tests/host/win32/gl/test_gl.py`. Each row lists:
   - the family name;
   - the compile define;
   - the backend sources;
   - the engine file;
   - which units it replaces.

   Build Savage (the default), Radeon and Matrox through it, and later Mach64 and VMware SVGA3D. `tests/host/win32/gl/quake3.c` and `kernel_engine.c` use `#if defined(GL_KERNEL_FAMILY_X)` blocks per family, and Savage is the `#else` default.
2. **Status table and contract.** The status table in `tests/host/test_render3d.py` lists every family. The 3D contract keeps every family's section, numbered in family order.
3. Integration 8 is the reference for Savage and shared code. Keep its code exactly and justify every removed line in the report.
4. **Tests.** Run, for every family present:
   - its own tests;
   - `tests/host/test_render3d.py`;
   - the display driver model tests;
   - the Savage family tests;
   - `python3 -B -m unittest tests.host.win32.gl.test_gl`.

   Record the results. LeakSanitizer failures caused by the sandbox are rerun by the lead.

## Rules

- Never stage or commit.
- Never run QEMU, `systemd-run` or image builds.
- Run tests sequentially with `nice -n 19`.
- Report to `build/codex/f7-unify-report.md` and append a section on each later round.
