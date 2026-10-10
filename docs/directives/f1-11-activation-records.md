# Directive f1-11: driver activation evidence must follow the probe's BEGIN

- **Step:** F1. **Contracts:** `f0-acceptance.md` (evidence grammar: a
  probe's records start with BEGIN; the parser rejects any record before
  it), `f1-acceptance.md` (activation records belong to the selected probe
  and to `safe`/`registry`), the f1-08 report.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-activation`. Files: `src/kernel/core/init.c`,
  `src/kernel/include/ciuki/init.h`, `src/kernel/probes/probes.c` (boot
  probe only), `src/kernel/probes/safe_probe.c`, `src/kernel/probes/registry_probe.c`
  (read the ledger instead of records), `tests/host/runtime_init_test.c`.
  Nothing else.

## Defect

On image `0ba29b3e…` (commit `9a09809`, first image with f1-08)
`f0-smoke` fails at once: `drivers_init` emits
`CIUKI_TEST … probe=boot event=DATA group=activation_flag …` before the boot
probe has emitted BEGIN, so the runner reports "unknown event or missing
BEGIN" and stops. No F0 probe can pass on this image.

## What to do

1. `drivers_init` writes no `CIUKI_TEST` record. It appends every step to a
   bounded in-kernel **activation ledger** (`struct activation_entry {seq,
   device, result, reason, tick, safe_flag_before}`, 32 entries) and logs
   each step with `klog` (`[init] fbdev … result=…`) for the human serial log.
2. The F0 `boot` probe, after its BEGIN and before READY, emits one
   `DATA group=activation …` record per ledger entry (same fields as the
   records f1-08 emitted, now in the right place) and one
   `group=activation_summary devices=… failures=… optional_activations=…`
   record; a probe other than `boot` emits nothing from the ledger except
   `safe` and `registry`, which already consume it (they read the ledger,
   not the records).
3. Sequence numbers: the first record of any run is the selected probe's
   BEGIN (seq 1), as before f1-08.
4. The ordinary boot without a selector keeps the klog lines only.

## Host tests (mandatory)

Extend `tests/host/runtime_init_test.c`: the ledger is filled in order with
the gating cases of f1-08; a fake record sink proves that `drivers_init`
emits zero records and that the boot probe's emission order is BEGIN,
activation records, summary, then the existing boot records; the 240-byte
bound holds for every activation record.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f0-smoke` 2/2 and `f0-core` 56/56 with
the drivers started at boot, and the activation records visible after
BEGIN. Reply with: files, test output, and any contract problem found.
