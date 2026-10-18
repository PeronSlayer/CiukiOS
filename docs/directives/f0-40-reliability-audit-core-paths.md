# f0-40 — reliability audit of the paths that must always work: boot, input, storage, display, USB, audio, VM

- **Phase:** F0 (cross-cutting). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f0-40` from main (after `06c3d1fa`).
- **Authority:** read everything; change only what the audit shows with evidence, in small verifiable steps, each with a host test: `src/boot/ciukldr/` (menu/deadline/progress), `src/kernel/core/` (display restore, registry, bootlog, scheduler stretches), `src/kernel/drivers/supersavage_*.c`, `src/kernel/usb/`, `src/kernel/fs/`, `src/kernel/audio/` (coordinate with f4-31: diagnostics only), `src/kernel/vm/` (coordinate with f3-44: no VIF changes), `docs/design/*.md`.

## Owner directive (2026-10-18)

"Use every resource you can, search the internet, review the main things of the system well: we must be sure they always work."

## Evidence to start from (read-only): the five T23 trials `legacy/local/physical/{d598bf4c,43d72a5c,041b221f,eca4d773,9af46936}/BOOT*.LOG` and the directives f0-33…f0-39, f1-78…f1-81, f3-36…f3-43, f4-30. Known open items:

- Loader: one QEMU run (`build/test-runs/f0-smoke/6037672f`, image 2fc3c429) stayed at the menu hint screen for 300 s without loading the kernel (not reproduced). The loader has no progress watchdog or diagnostic after the menu.
- Display: native full-screen restore still ends with `native_supersavage phase=restore result=-5 stage=native-restore-streams reg=0` (9af46936), recovered by the VBE fallback, but each costs a strike and three strikes disable native full screen for the boot (f3-39).
- Serial: `loader_uart state=2` on every T23 boot (BIOS port disabled) — the disk log is the only evidence channel: every critical path must leave protected records on it.
- IRQ 11 cascade (fixed by f0-39), USB lease loss, SB16 waits (f3-40), VFS lock (f1-78/79), presenter stretches (f3-41), full-screen return stall (f1-80): verify each fix has a regression test that exercises the exact failure and that the diagnostics would name the culprit next time.

## Required

1. Write `docs/validation/f0-40-reliability-audit.md`: for each path (boot and loader, input PS/2+USB, boot volume and removable storage, display mode set/restore/full screen, USB controllers, audio transport, DOS VM lifecycle, shutdown/reboot) list: the invariant ("always works" means…), the single points of failure, the timeouts and their fallbacks, the quarantine policies and whether a quarantine can take an unrelated function down, the diagnostics emitted, and the regression tests that exist. Research the standard practices (Linux kernel watchdog/`hung_task`, Windows 9x/NT fail-safe boot, SeaBIOS boot order) as references.
2. Fix with evidence, each as a separate commit-sized change with a host test: (a) loader progress watchdog: after the menu every stage prints a short serial/screen token and has a bounded deadline; a stage that exceeds it logs `L:STALL <stage>` and continues or falls back (document); (b) `native-restore-streams` result -5: diagnose from `supersavage_mode.c` what the streams stage compares (`reg=0 mask=0`: a timeout?), fix the readback/wait so a healthy restore returns 0 on the T23 recipe, and stop counting a recovered restore as a strike unless the fallback also failed; (c) any quarantine that can disable an unrelated function (list them; the IRQ-line case is fixed; check registry cascades, display check, storage) gets member isolation; (d) every path's failure leaves a protected record naming the stage.
3. Add `tests/host/reliability/` (or extend existing suites) with one test per invariant that injects the historical failure (from the trials) and asserts the fallback and the record.
4. Host suites: full `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, loader tests, `python3 scripts/build_kernel.py`, `make CAP= apps`.
5. Report `build/codex/f0-40-report.md`: the audit table, the changes, what remains (prioritised) for the lead.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Do not touch the audio transport logic (f4-31) or the VIF logic (f3-44) beyond diagnostics. No new register programming without a primary source.
