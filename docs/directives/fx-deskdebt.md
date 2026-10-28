# fx-deskdebt: desktop suite failures on main

- **Written:** 2026-10-27 20:40 by the lead. Implementer: Claude agent. Base: main 12641a954 or later.
- **Evidence:** several agents report the same desktop-suite failures on unmodified main, so they block the final chain: `test_wordpad` and `test_schedtask` screenshot comparisons (the fx-icons merge changed icons without re-recording them), `test_catalog_registration` (looks only in the Tango archive while `catalog.h` now names Haiku icons), `test_add_remove`, `test_accessories`, `test_wallpaper_startup`, `test_actual_owner_provider_partitioned`, `test_startup_logs_do_not_coalesce`, and the three `test_desktop` payload/provenance tests that need a fresh `make CAP= apps`.

## Scope

Run `python3 -B -m unittest discover -s tests/host/desktop -t .` (through the lead's heavy.sh) on a fresh `make sdk` + `make CAP= apps` of this worktree and make it green. For each failure decide whether the product or the test is wrong: re-record a golden only when the new picture is the intended result of a merged change (say which, and look at the PNG before accepting it); fix product code when it is a regression; extend tests to the new icon sets where the test is narrower than the design (`docs/design/os-feature-inventory.md`, `assets/icons/README.md`). List every failure with its cause and fix in the report.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-deskdebt` (branch `wt/fx-deskdebt`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-deskdebt-report.md` in the worktree. Leave changes uncommitted.
