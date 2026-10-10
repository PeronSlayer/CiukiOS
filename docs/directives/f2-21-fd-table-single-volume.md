# Directive f2-21: `fd-table` on a single-volume machine

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`fd-table` row:
  `EXDEV` for cross-volume rename and the read-only second volume are
  fixture-backed T3 subcases), `test-architecture.md` (hardware tier has
  the boot disk only), directive f1-30 (same rule for `fat-read`).
- **Implementer:** Codex, `gpt-6-luna`, effort `medium`.
- **Worktree:** `wt/f2-fd-table-hw`, from `main` at or after `abcd677`.
  Files: `src/kernel/probes/f2_probes_files.c`, `tests/host/proc/files_test.c`,
  `tests/host/test_runner_f2_alignment.py`, `tests/suites/f2-process.json`
  only if a record changes, `scripts/test/physical.py`, one line in
  `f2-acceptance.md` stating the tier of the two subcases.

## Observed on the ThinkPad T23 (sweep `66666666`, 2026-10-11)

```
probe=fd-table … ERROR case=exdev status=not_run reason=missing_second_volume
probe=fd-table … END status=FAIL reason=files_contract
```

Every other operation of the probe matched; the only miss is the
fixture-backed pair (`exdev`, `readonly`), which needs the FAT16 fixture
disk the runner attaches on QEMU and that no hardware target carries.

## Required behaviour

1. When no second volume is mounted, the probe records
   `case=exdev status=not_run reason=second_volume_absent` and
   `case=readonly status=not_run reason=second_volume_absent` and does
   not fail the verdict for that reason; with a second volume the
   behaviour is unchanged (QEMU cases keep their fixture).
2. The physical import maps the two subcases to `not_run` with the
   reason; the contract line names them as T3 subcases.
3. Host tests: with and without a second volume.

## Acceptance by the lead

Host tests; QEMU `fd-table` on three profiles unchanged; on the T23
sweep `fd-table` passes with the two subcases `not_run`.
