# Directive f1-17: align `f1-fat32` and `f1-storage` with the FAT probe records

- **Step:** F1. **Contracts:** `f1-acceptance.md` (probe table: `fat-read`,
  `fat-write`, `cache`, `mount-crash`, `bootlog`, `ata-fault-blkdebug`
  evidence fields), the f1-09 and f1-16 reports.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-fat-suites`, from `main` after f1-16 is merged.
  Files: `tests/suites/f1-fat32.json`, `tests/suites/f1-storage.json`,
  `src/kernel/probes/fat_probes.c` (only to add contract-required summary
  records the probe does not emit yet, never to remove evidence),
  `src/kernel/probes/registry_probe.c`/`ata_probe.c` only for the same
  reason, `scripts/test/run.py` (the `ata-fault-blkdebug` case is still
  declared `not_run`: make it run the blkdebug read-error injection the
  f1-03 directive specified and compare the guest's `EIO`/quarantine
  records), `tests/host/test_runner.py`.

## Observed

f1-16 reports that the `f1-fat32` predicates expect summary/LFN records and
fixture `name_hex` digest records the production probe does not emit; the
`ata-fault-blkdebug` case ends `not_run` by declaration.

## What to do

1. For each FAT probe, take the records it emits (host run of the probe
   through the storage harness) and the evidence fields the contract table
   requires; where the contract requires a field the probe lacks (for
   example a per-volume summary with name/alias/size error counters, LFN
   orphan handling counters, write counts), add the record to the probe;
   where the suite invented a field (such as `name_hex` digests of names
   instead of UTF-8 names and SHA-256 file digests), change the suite to
   the probe's representation. The runner's own digest computation
   (`mtools`, `hashlib`) stays the comparison source.
2. `ata-fault-blkdebug`: implement the case end-to-end (blkdebug
   `read_aio` error on the overlay at a declared LBA after `ARM`, guest
   `ata` read returning `EIO`, quarantine recorded, zero further commands).
3. Host tests: predicate tests from the probes' real record formats for
   every `f1-fat32` and `f1-storage` case; the blkdebug argument builder.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f1-fat32` and `f1-storage` pass on
`qemu-t23` with the FAT12/FAT16 fixtures attached. Reply with: files, the
per-probe record tables, test output, and any contract problem found.
