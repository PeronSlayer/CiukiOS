# fx-files-nav-merge: resolve the merge of main into fx-files-nav

- **Written:** 2026-10-29 10:44 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-files-nav`, branch `wt/fx-files-nav`.
- **Merge in progress:** the lead started `git merge main`. `main` now has fx-desk-arrange and fx-semver. The only conflicted file is `apps/files/app.c`.

## Scope

1. Resolve `apps/files/app.c` and keep both sides completely:
   - fx-files-nav: address-bar history and autocomplete, the Back/Forward drop-downs, the Undo toolbar button, the extension-change warning, column widths and auto-fit, Arrange Icons and Auto Arrange in Files;
   - fx-desk-arrange: shared system-icon display names and the sort/arrange code it changed.

   Where both sides added an Arrange or sort implementation, keep one shared implementation used by both the desktop and Files, and justify the choice in the report. No feature of either side may be lost. The lead runs `lost-lines.py` after the merge.
2. Check the non-conflicting files that both branches touched (`apps/libdeskui/*`, `tests/host/abi_layout_test.c`, `tests/host/desktop/*goldens*.json`) for semantic clashes. For example, two new ABI records need two `abi_layout` rows, and goldens must be regenerated only through their documented generator.
3. Run sequentially with `nice -n 19`:
   - the Files and desktop host tests from both directives;
   - `tests/host/test_display_driver_model`;
   - `python3 scripts/gen_api_reference.py --check`.

## Rules

- Resolve conflict markers and edit files only. Never run `git add`, `git commit` or `git merge`.
- Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-files-nav-merge-report.md`.
