# f7-unify2: the remaining GPU families on the unified family harness

- **Written:** 2026-10-29 09:41 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-integ10`, branch `wt/integ10` from `main` (`b64539fd7`). `main` already has f7-unify: one family table for Savage, Radeon, Matrox, Mach64 and VMware SVGA3D.
- **Merge in progress:** the lead has started `git merge wt/f7-21` (Intel 810/815). The 15 conflicted files are listed by `git diff --name-only --diff-filter=U`.
- **Next merges:** after you, the lead merges, in this order, each one queued to you again with this directive:
  1. f7-22 (Intel 830–865);
  2. f7-23 (ATI Rage 128);
  3. f7-28 (3dfx Voodoo);
  4. f7-29 (SiS 300);
  5. f7-30 (VIA Unichrome, partial);
  6. f7-32 (S3 ViRGE, partial).

  These branches date from 2026-10-26/27 and predate f7-unify. Their shared-code edits were written against the older render3d layout.

## Scope

1. **Resolve every conflict onto main's layout.** `main` is the reference for the shared 3D code: `render3d*.c`, `abi.h`, the opengl32 admission and encode headers, the gdi32 render3d path and the developer API.
   - Keep main's code exactly. Add only what the family needs.
   - Port each family's backend, its probe gate and its tests into the f7-unify family table (`tests/host/win32/gl/test_gl.py`, the `#if defined(GL_KERNEL_FAMILY_X)` blocks in `quake3.c` and `kernel_engine.c`).
   - Justify every removed line of either side in the report.
2. **ABI.** New ABI constants or records keep one field per line and get their `abi_layout` rows. Regenerate `docs/developer-api.md` with `scripts/gen_api_reference.py`. Never hand-merge it.
3. **Status table and contract.** Add the family to the status table in `tests/host/test_render3d.py`. Add its section to the 3D contract in family order, keeping its partial or gated status exactly as its branch recorded it.
4. **Kernel test list.** Merge `scripts/test/host_kernel_tests.sh` so that both sides' tests run.
5. **Tests, run sequentially with `nice -n 19`:**
   - the family's own tests;
   - `tests/host/test_render3d.py`;
   - the display driver model tests;
   - the Savage family tests;
   - `python3 -B -m unittest tests.host.win32.gl.test_gl`.

   Record the results. LeakSanitizer failures caused by the sandbox are rerun by the lead.

## Rules

- Resolve conflict markers and edit files only. Never run `git add`, `git commit` or `git merge`; the lead does.
- Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/f7-unify2-report.md`. Give one section per merged family.
