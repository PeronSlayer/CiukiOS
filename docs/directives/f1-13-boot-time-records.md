# Directive f1-13: no `CIUKI_TEST` record before the selected probe's BEGIN

- **Step:** F1. **Contracts:** `f0-acceptance.md` (evidence grammar: the
  first record of a run is the selected probe's BEGIN), `f1-acceptance.md`
  (activation and storage evidence belong to probes).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-boot-records`, from `main` after f1-12 is merged.
  Files: `src/kernel/fs/mount.c`, `src/kernel/core/init.c`,
  `src/kernel/include/ciuki/init.h`, `src/kernel/core/bootlog.c`,
  `src/kernel/proc/supervisor.c`, `src/kernel/probes/probes.c` (boot probe
  emission only), `src/kernel/probes/fat_probes.c`, `src/kernel/probes/safe_probe.c`,
  `tests/host/*`, `scripts/test/host_kernel_tests.sh`. Nothing else.

## Defect

Image `0e4261b5…` (commit `5a654a7`): `storage_init` emits
`CIUKI_TEST … probe=boot event=DATA group=storage disk=0 result=0 gate=read
writes=0` during `drivers_init`, before the boot probe's BEGIN. Every F0 suite
fails with "unknown event or missing BEGIN" (or "version, run or probe
mismatch" for `panic`). This is the second occurrence of the same class
(f1-11 fixed the activation records).

## What to do

1. `storage_init` (and anything else called from `drivers_init`, including
   `supervisor_bootstrap` and the boot log) emits **no** `CIUKI_TEST`
   record: it appends storage entries to the activation ledger (new entry
   kinds `storage` with disk, partition, mount mode, gate, write count,
   qualified flag/reason) and writes `klog` lines.
2. The boot probe emits the storage ledger entries after its BEGIN, next to
   the activation records (`group=storage …`, same fields), and the
   `fat-*`/`bootlog`/`safe` probes read the ledger for their mount facts.
3. **Static guard**: a host test scans `src/kernel/` and fails if
   `rec_emit`/`rec_emit_panicsafe` is called from any file outside
   `src/kernel/probes/`, `src/kernel/drivers/*_probe.c` and `output.c`
   itself (list the allowed files explicitly; keep the list in the test).
4. **Runtime guard**: `rec_emit` before `rec_set_run`/the first BEGIN of a
   run counts a `premature_records` counter that the boot probe reports
   (`premature_records=0` expected) instead of writing the record; `klog`
   notes the dropped record once.

## Host tests (mandatory)

The static guard; the ledger with storage entries in order; a fake-sink
test proving `drivers_init` with storage emits zero records and the boot
probe's order is BEGIN, activation, storage, summary, boot records; the
runtime guard counter.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f0-smoke` 2/2 and `f0-core` 56/56 with
storage mounted at boot, `group=storage` records visible after BEGIN with
`premature_records=0`. Reply with: files, test output, and any contract
problem found.
