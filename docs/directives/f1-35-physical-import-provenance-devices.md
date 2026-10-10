# Directive f1-35: physical import, damaged provenance line and QEMU-only device exceptions

- **Step:** F1 (test infrastructure). **Contracts:**
  `test-architecture.md` (hardware tier: evidence rules, every refusal
  explicit), directives f1-28 and f1-33 (importer),
  `docs/build-and-run.md` (import procedure).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-import-hw2`, from `main` at or after `857b5b7`.
  Files: `scripts/test/physical.py`, `tests/host/test_physical.py`,
  `tests/host/test_runner.py`, `docs/build-and-run.md`,
  `docs/design/test-architecture.md` (one paragraph). The real capture is
  in `build/t23/77777777/` (`serial.log`, `acquisition.json`) of the
  worktree; install the boots needed as the local fixture
  `tests/host/fixtures/physical/77777777/` (`f0.log` + `acquisition.json`;
  `*.log` files are ignored by git and the tests must skip without them,
  as for `66666666`).

## Observed (2026-10-11)

1. `run.py f2-all --physical-capture legacy/local/physical/77777777
   --image-sha256 6ed146ab…` is refused with `sweep boot lacks BOOT.CFG
   provenance`. The capture (35 boots, `SWEEP_END passed=29 failed=4
   not_run=2`) has 34 `L:SELECT_SOURCE=` lines; in the boot after step 18
   (`cache`) the line reads `L:SELECT_SOURCE=fg` and the preceding loader
   line begins with a garbled byte: bytes were lost at the self-reboot.
   The check in `physical.py` (`b'L:SELECT_SOURCE=cfg' not in
   boot.splitlines()`) refuses the whole capture for one damaged line.
2. The import of `66666666` marks `video-fallback` FAIL. The case
   (`tests/suites/f0-core.json`, probe `boot`, profile `qemu-t23`)
   declares `device_exceptions: {vga: none}` and a predicate on
   `group=video` `text eq 1`: it describes a QEMU boot without a VGA
   device, and the physical machine has an LFB, so the records of the
   real `boot` probe cannot satisfy it.

## What to do

1. **Provenance rule.** A sweep boot with an intact provenance line
   passes as now. A sweep boot whose line is damaged (a
   `L:SELECT_SOURCE=` line whose value is not `cfg`, or no such line while
   the boot shows serial damage: damaged records or a sequence gap at its
   start) is accepted only when the sweep continuity proves the selector
   came from the on-disk cursor: the same run identity and a
   `probe=sweep` step equal to the previous boot's step plus one (or the
   repeated step of a two-boot probe). `boot_evidence` then records
   `provenance: 'damaged'` with the raw line and
   `provenance_inferred_from: 'sweep continuity'`. Without continuity the
   boot's results are `not_run reason=provenance_damaged`. An intact line
   with another value (for example `menu`) inside a sweep remains a
   refusal. Never accept silently.
2. **Device exceptions.** A case whose `device_exceptions` (or any other
   QEMU-only machine variant key) cannot hold on the physical machine is
   `not_run reason=device_exception_not_applicable:<key>=<value>`, not
   FAIL, unless `acquisition.json` declares the matching physical
   configuration (`devices: {vga: none}`), in which case the predicates
   apply. The physical summary lists these cases separately.
3. **Host tests:** the damaged-provenance boot (real fixture `77777777`),
   a synthetic sweep boot with a damaged line and no continuity
   (`not_run`), a synthetic intact non-`cfg` value (refusal), the
   `video-fallback` mapping, and the changed `66666666` result
   (`video-fallback` `not_run`, no FAIL).
4. Document both rules in `docs/build-and-run.md` (import) and in one
   paragraph of `test-architecture.md`.

## Acceptance by the lead

Host tests; `run.py f2-all --physical-capture legacy/local/physical/77777777
--image-sha256 6ed146ab…` imports the capture (35 boots, summary written)
and the `66666666` import shows `video-fallback` `not_run`. Reply with the
two rules and the summary tables.
