# Directive f1-37: loader menu — safe mode followed by a probe selector (S then P)

- **Step:** F1 (hardware qualification of safe mode). **Contracts:**
  `boot-memory.md` (loader, `ciuki_boot_info` flags, selector source),
  `f1-acceptance.md` (`safe` row and the `safe-menu-*` physical cases),
  `test-architecture.md`, directive f1-36 (open point: the loader cannot
  combine S and P today; smallest change proposed in its report and in
  `docs/build-and-run.md`, "Operator safe-mode boot after a sweep").
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-loader-safe-probe`, from `main` at or after `798a92d`.
  Files: `src/boot/ciukldr/menu.inc` (and `ciukldr.asm` only if the size
  budget needs it), `scripts/test/loader_model.py` and its tests (the
  loader model must reproduce the new key flow), `tests/host/*` loader
  stage tests, `docs/build-and-run.md` (replace the "not supported today"
  paragraph with the qualified procedure), `docs/design/boot-memory.md`
  (one sentence on the menu flow), `tests/suites/f1-safe.json` only if a
  QEMU case can exercise the flow through the runner's input script.

## Observed

In `boot_menu` (`menu.inc`), `.safe` calls `.cancel_cfg`, sets
`CBI_F_SAFE_MODE` and jumps to `.done`; `.probe` reads a selector within
its 30-second entry deadline but adds no safe flag, and the kernel accepts
the selector suffix ` safe=1` only under QEMU (`selector.c`). The operator
therefore cannot boot the hardware in safe mode *and* run `f1:safe run=…`
without rewriting the disk, which the physical cases `safe-menu-t23` and
`safe-menu-e500` require.

## What to do

1. After `S`, set the safe flag, cancel any `BOOT.CFG` test request as
   today, print a one-line confirmation (`Safe mode selected; P for a
   probe, or wait`), restart the three-second deadline and return to
   `.poll`, so that `P` (and `L`) can follow. `N` after `S` keeps safe mode
   (it only declines to add it). Expiry boots safe mode without a probe.
   `P` after `S` runs the existing selector entry; the kernel receives both
   the safe flag and the selector. The loader's size and CRC budget must
   still pass T1.
2. Record the chosen key flow in `L:` serial lines as today
   (`L:SELECT_SOURCE=menu`, plus a `L:SAFE=1` line when safe mode was
   selected from the menu) so the capture proves the source.
3. Loader model and tests: the Python loader model follows the same flow;
   host tests cover S→P, S→N, S→expiry, P alone and the deadline restart.
4. Guide: the exact operator sequence for the safe-menu acquisition
   (`S`, `P`, `f1:safe run=<fresh id>`, Enter) in `docs/build-and-run.md`.

## Acceptance by the lead

Host tests and T1; QEMU `f1-safe` unchanged; on the T23, a serial capture
of one safe-menu boot imports as `safe-menu-t23` PASS (operator confirmed).
