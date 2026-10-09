# Directive f1-02: F1 suites, input injection and the `f1:` selector

- **Step:** F1. **Contracts:** `f1-acceptance.md` (Evidence protocol
  additions, Runner and suite additions), `f0-acceptance.md` (grammar,
  runner rules), `test-architecture.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-runner`. Files: `scripts/test/run.py`,
  `scripts/test/qmp.py`, `scripts/test/evidence.py`,
  `scripts/test/loader_model.py`, `tests/suites/f1-*.json`,
  `tests/host/test_runner.py` and `fake_qemu.py`, `src/boot/ciukldr/menu.inc`
  (probe-name list and `f1:` prefix only), `src/kernel/probes/probes.c`
  (`parse_selector` only: accept `f1:` and the F1 probe names, dispatching
  to a table that is empty for now). Nothing else.

## What to build

1. **Selector:** grammar `f[01]:<probe-id|all|core> run=<8 hex>
   [platform=e500] [safe=1]`; F1 probe ids from the contract table
   (`registry`, `input`, `input-fault`, `framebuffer`, `ata`, `ata-fault`,
   `partition`, `fat-read`, `fat-write`, `cache`, `mount-crash`, `safe`,
   `bootlog`); the loader's `probe_names` list and the kernel parser accept
   them; the evidence parser's `PROBES` set too. Unknown or duplicate keys
   still run no probe.
2. **Runner actions** declared per case in the suite JSON: wait for a
   `READY`/`ARM` record, then QMP `input-send-event` sequences (key `qcode`
   make/break paced below the typematic delay, relative `rel` x/y, `btn`),
   with drain pauses between batches; `blkdebug` fault injection on the
   overlay for declared ATA/cache faults (record which layer failed);
   overlay `BOOT.CFG` patching with an exact patch manifest (bytes, offset,
   SHA-256 before/after); a declared reboot sequence reusing one overlay;
   stop QEMU before exporting the overlay read-only for `fsck.fat` and
   `mtools` checks; `cache=unsafe` forbidden; backing-image SHA-256 verified
   before and after.
3. **Suites:** `f1-input.json`, `f1-storage.json`, `f1-fat32.json`,
   `f1-safe.json` with the probes, predicates and deadlines of the
   contract (input 120 s, storage 180 s, FAT 300 s, safe 90 s), on the
   icount profiles; an `all` alias that expands into ordered individual
   selectors across boots; F0 regressions first.
4. **`result.json` additions:** stimulus and fixture hashes, fault/cut
   point, disk cache mode, checker versions and results, durability
   observations.

## Host tests (mandatory)

Extend `tests/host/test_runner.py` and `fake_qemu.py`: fake QMP that
records `input-send-event` calls and `blkdebug` arguments; grammar tests for
`f1:` selectors (valid, unknown probe, duplicate key, oversized); overlay
patch manifest round trip; reboot sequence on one overlay; read-only
export ordering (QEMU stopped before `fsck.fat`). Keep the 30 existing
tests green.

## Acceptance by the lead

Diff review; host tests pass; the loader still assembles and the kernel
builds; on QEMU the F0 suites still pass and a dry `f1-input` run reaches
`READY` then fails cleanly with `not_run` for the missing F1 probes (the
kernel table is empty until the driver directives land). Reply with: files,
the final grammar regex, suite case ids, test results, and any contract
problem found.
