# Directive f1-32: the sweep's cursor write makes the read-only probes fail

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`fat-read` row: no
  writes during the read probe; `bootlog` reopen requires zero volume
  writes before qualification), `vfs-storage-contract.md`, directive
  f1-28 (cursor written to `\SYSTEM\BOOT.CFG` before each step; known
  limit "bootlog reopen sees the cursor write").
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-sweep-writes`, from `main` at or after `abcd677`.
  Files: `src/kernel/fs/mount.c` (write accounting), `src/kernel/probes/probes.c`
  (sweep cursor writer), `src/kernel/probes/fat_probes.c` (read probes'
  write checks), `src/kernel/core/bootlog.c` only if the reopen check
  must use the same baseline, `tests/host/fs/*`, `tests/host/test_sweep.py`,
  `scripts/test/physical.py` if a record changes.

## Observed on the ThinkPad T23 (sweep `66666666`, 2026-10-11)

`fat-read` ends `FAIL error=-5` on both sweeps although every record it
emits is fine and no fixture disk exists (the f1-30 `not_run` path is
never reached): in `probe_fat_read` the check
`if (v->error || !v->fat.readonly || v->writes) e = -FS_EIO` sees the
live `v->writes` incremented by the sweep's own cursor write to
`BOOT.CFG`, performed just before the probe runs. The same happens to
any probe that asserts "no writes before my gate" (the `bootlog` reopen
case already documented), while the activation snapshot in the records
still says `writes=0`.

## Required behaviour

1. The storage layer accounts the sweep cursor writes separately
   (`v->sweep_writes` or a baseline captured when the probe starts):
   read probes compare writes against their start baseline, and the
   records state both (`writes_before_probe=<n> writes_during=<m>`).
2. `bootlog` reopen: the zero-writes requirement applies to writes other
   than the cursor; document the rule in the contract paragraph the
   f1-28 report already added and remove the "known limit".
3. Host tests: a read probe run after a cursor write passes; a real
   stray write during the probe still fails.

## Acceptance by the lead

Host tests; QEMU `f1-fat32` unchanged and `sweep-smoke` PASS; on the T23
sweep `fat-read` becomes `not_run reason=fixtures_absent` and `bootlog`
reopen passes.
