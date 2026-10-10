# Directive f1-33: the physical import must accept a real sweep capture

- **Step:** F1 (test infrastructure). **Contracts:** `test-architecture.md`
  (hardware tier import: one capture per sweep, per-case results,
  prerequisites, operator-confirmation cases), directive f1-28 and its
  runner follow-up, `docs/build-and-run.md` (import procedure).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-import-real`, from `main` at or after `abcd677`.
  Files: `scripts/test/physical.py`, `scripts/test/run.py` (import path
  only), `tests/host/test_physical.py`, `tests/host/test_runner.py`,
  `docs/build-and-run.md` (the `acquisition.json` fields, with
  `build_id`), a test fixture made from the real capture under
  `tests/host/fixtures/physical/` (trimmed to the boots needed, no
  private material).

## Observed (2026-10-11)

Two unattended sweeps on the ThinkPad T23 produced real captures
(`44444444`: 33 boots on image `cb33aec3…`; `66666666`: 35 boots to
`SWEEP_END passed=27 failed=6 not_run=2` on image `90477716…`). Importing
`66666666` with `run.py f2-all --physical-capture <dir>` was refused in
turn by `embedded build identity missing` (the documented field list
omits `build_id`), then `sequence must increase within each boot`. The
capture is a byte-exact serial log at 38400 8N1 with the loader lines,
klog lines, `[console]` application lines, the CIUKI_TEST records of 35
boots, two operator power-cycles and the records of two-boot probes
(`fat-write`, `mount-crash`, `bootlog`), plus the kernel's self-reboot
between steps. The capture is in `build/f1-33/66666666/` of the worktree
(`serial.log`, `acquisition.json`); the earlier one in `build/f1-33/44444444/`.

## What to do

1. Make the import succeed on the real `66666666` capture: find why the
   sequence check fails (boot splitting on the loader banner versus the
   kernel banner, the sweep's own `probe=sweep` records, the
   self-reboot without a banner, serial byte loss or line splits such as
   the `[console]` lines, carriage returns) and fix the splitter or the
   check so that genuine per-boot monotonic sequences are verified and
   any real gap is recorded as evidence (`records_lost=<n>` for that
   boot) instead of refusing the whole capture.
2. Produce the per-case results for the physical profile: every case of
   the `f2-all` suites mapped from the sweep records (including the
   `not_run` reasons `fixtures_absent`, `second_volume_absent`,
   `operator_absent`, `reset_before_completion`), the prerequisite rule
   per case, one summary; the earlier capture `44444444` (image no longer
   canonical) must be importable with an explicit `--image-sha256`
   override that records the mismatch as evidence of a historical image.
3. Document the full `acquisition.json` schema in `docs/build-and-run.md`
   (all required keys, `build_id`, `panic_observations`).
4. Host tests: the real-capture fixture through the importer; a
   synthetic capture with a byte gap; the override path.

## Acceptance by the lead

Host tests; `run.py f2-all --physical-capture legacy/local/physical/66666666`
writes the physical summary with the expected per-case outcomes; the
`44444444` capture imports with the override. Reply with: the splitter
rule, the summary table of the `66666666` import, test output.
