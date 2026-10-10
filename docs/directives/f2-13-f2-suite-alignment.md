# Directive f2-13: align `f2-process`, `f2-runtime` and `f2-app` with the probes' real records

- **Step:** F2. **Contracts:** `f2-acceptance.md` (probe table rows
  `elf-load`, `spawn-wait`, `fd-table`, `mmap`, `signals-fault`,
  `threads-wait`, `libc-smoke`, `app-gate`; suite table; retained evidence),
  `test-architecture.md`, the f1-17 precedent (FAT suites aligned with the
  probe records, never by removing evidence).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f2-suite-alignment`, from `main` at or after `c9cd430`.
  Files: `tests/suites/f2-process.json`, `tests/suites/f2-runtime.json`,
  `tests/suites/f2-app.json`, `scripts/test/run.py` (only predicate/checker
  machinery the alignment needs), `tests/host/test_runner.py` and sibling
  runner tests, the F2 probes `src/kernel/probes/f2_probes_*.c` **only to
  add a contract-required record or field the probe does not emit yet,
  never to remove or rename evidence**, `sdk/tests/libc_smoke.c` and
  `apps/lua/*` only for the same reason. Do not touch
  `tests/suites/f2-desktop.json` or `f2_probes_desktop.c`: directive f2-12
  owns them.

## Observed on image `a9550f04…` (commit `3619267`, 2026-10-11)

- f2-09 wrote the F2 suite predicates from the acceptance table and tested
  them against fabricated record sets; no F2 suite had run on QEMU. The
  first real run of `fd-table-qemu-t23` (with the FAT16 fixture disk added
  by commit `c9cd430`) ends with the kernel `END status=PASS` and the runner
  `FAIL … missing evidence` on the combined `group=fd-table` predicate
  (`fd_slots`, `emfile`, `leaked`, `offset_errors`, …, `required_fields`
  `directory_digest`, `identity`, `link_counts`, `flags`,
  `checker_digest`): the probe emits per-operation
  `operation=<name> expected=<v> observed=<v>` records, `case=user`,
  `case=clock-*`, `case=durable-*`, `case=ledger`, `case=post-commit-fault`
  and `case=durable-checker` records instead.
- The real records of every probe on this image are in
  `build/f2records/<probe>.records` of the worktree (one file per probe:
  all `CIUKI_TEST` lines of one boot on `qemu-t23`; `fd-table` was captured
  through the runner with its fixture disk, the others by single boots).
  `app-gate.records` is included when the lead's boot finished in time;
  otherwise derive its predicates from the probe source and say so.

## What to do

1. For each probe of the three suites, take the real records and the
   evidence fields the contract row requires. Where the contract requires a
   field or summary the probe lacks, add the record to the probe (host test
   for the new record); where the suite invented a field, a grouping or a
   name the probe never emits, change the suite to the probe's
   representation. The runner's independent computations (digests through
   `mtools`/`hashlib`, fixture manifests, ELF hashes) stay the comparison
   source where the contract asks for them.
2. Keep the three profiles per case and the fd-table fixture disk. Check
   every `checks`/`digests` entry the same way (host-side checkers must
   refer to files the probe really writes, with the names it really uses).
3. Host tests: predicate evaluation against the **real** record files for
   every case of the three suites (copy the record files under
   `tests/host/fixtures/` or generate them from the probe source in the
   test), plus the existing fabricated-set tests updated to the real shape.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f2-process`, `f2-runtime` and `f2-app`
pass on `qemu-t23` (the lead runs them with their F0/F1 prefix). Reply with:
files, a per-probe table "contract field → record/field used → suite
predicate", any contract field that no record can supply, test output.
