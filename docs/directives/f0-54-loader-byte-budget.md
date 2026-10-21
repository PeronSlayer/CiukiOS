# f0-54 — loader byte budget: free at least 512 bytes inside `ciukldr`'s fixed extent without behaviour change

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`
- **Written:** 2026-10-21 by the lead. Three branches of the day (f1-88 `accel=0`, f1-87 `usbscan=0`, f5-15A2's installer selector) each pushed `src/boot/ciukldr.asm` past its fixed extent (`TIMES value -28 is negative` after one merge; f5-15A2 recovered 16 bytes by shortening a prompt). The loader's size is part of the boot contract (`docs/design/boot-memory.md`; the `test_loader_image_keeps_its_fixed_size_and_hint` test). Instead of shaving strings at every merge, free a real budget once.
- **Worktree:** `../CiukiOS-wt-f0-54`, branch `wt/f0-54`
- **Read first:** `src/boot/ciukldr.asm`, `src/boot/ciukldr/*.inc` (menu, selector table, option tokens, UART, E820, VBE paths), `scripts/test/loader_model.py`, `tests/host/test_boot_modes.py`, `docs/design/boot-memory.md` (the extent, the `L:` records, the menu contract in `docs/build-and-run.md`).

Required:
1. Measure: a host tool (`scripts/loader_budget.py`) that assembles the loader and reports the bytes used per section (code, tables, strings, padding) and the free bytes; it becomes a test (`test_boot_modes`) that asserts `free >= 512` after this directive and prints the number on every run.
2. Free the budget with behaviour-preserving changes only, in this order of preference: a compact encoding of the option-token and selector tables (length-prefixed strings, shared prefixes such as `f3:`/`f5:`/`w32-`, one table instead of parallel ones), deduplicated message strings, shorter but still exact help text where `docs/build-and-run.md` allows (update the documentation in the same change), removal of dead code paths proven unreachable by the model; no change to any accepted token, record text (`L:` lines are matched by tests and the runner), timing, or menu semantics. The loader model (`loader_model.py`) stays the oracle: every existing test passes unchanged, except the size/budget test which gains the metric.
3. Record the before/after table in `docs/validation/f0-54-loader-budget.md`.

Verification: `test_boot_modes`, `test_runner_selectors`, `test_desktop_gate` green; `scripts/test/host_kernel_tests.sh` as far as the sandbox allows. Report `build/codex/f0-54-report.md`. `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; English.
