# Directive f2-11: `storage_sync` with the native namespace attached

- **Step:** F2 (integration seam with F1 storage). **Contracts:**
  `posix-subset.md` (cwd pins; EBUSY on synthetic roots and mountpoints),
  `vfs-storage-contract.md` (VFS namespace, open descriptions, detach),
  `f1-acceptance.md` (`fat-write`, `cache`, `bootlog` rows: durable shutdown
  through `storage_sync`), `f2-acceptance.md` (`fd-table` durable checker).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f2-storage-shutdown`, from `main` at or after `4dc9a0e`.
  Files: `src/kernel/proc/posixpath.c`, `src/kernel/proc/fdtable.c`,
  `src/kernel/include/ciuki/files.h`, `src/kernel/include/ciuki/vfs_hooks.h`,
  `src/kernel/fs/vfs.c`, `src/kernel/fs/mount.c`, `src/kernel/core/init.c`
  (only if the bootstrap order must change), `tests/host/fs/test_storage.c`,
  `tests/host/proc/files_test.c`, a new host test if needed,
  `scripts/test/host_kernel_tests.sh`, one amendment paragraph each in
  `docs/design/posix-subset.md` and `docs/design/vfs-storage-contract.md`.
  Do not touch the F1 probes (`src/kernel/probes/fat_probes.c`) or the suites.

## Observed on image `aee7f539…` (commit `408d056`, `qemu-t23`, single boots by the lead, 2026-10-11)

- `f1:fat-write`: every workload operation `result=0`, three file digests
  emitted, then `case=workload flush_result=-16`, `END status=FAIL error=-16`.
- `f1:cache`: writeback, eviction, block-driver errors, synthetic ATA
  quarantine, unsupported flush and shared-handle coherence all `result=0`;
  `END status=FAIL error=-16`.
- `f1:bootlog`: `case=qualification … writes=1 flush_result=0`
  (`bootlog_shutdown` succeeds), then `END status=FAIL error=-16`.
- In the `f1-fat32` suite run the `cache` failure left 13 cases `not_run`
  (prerequisite). `f2-process` `fd-table` calls `storage_sync()` the same way
  after `proc_collect()` and is expected to hit the same error.
- `tests/host/fs/test_storage.c` passes because it links neither
  `posixpath.c` nor `fdtable.c`: the weak `px_*` hooks are null on the host.

## Root cause (lead's reading; confirm it with a host reproduction)

`drivers_init` calls `files_bootstrap(&storage_get()->vfs)` (`init.c:138`)
on every boot. It attaches the namespace and pins the supervisor's cwd on
the root node (`px_retain`, `refs=1`, `volume == C:`). `storage_sync()` →
`storage_shutdown()` sets `s->stopped = true`, commits the volumes, then
`vfs_detach(C:)` asks `px_volume_busy()`, which returns true for any
namespace node with `refs` on that volume, so the detach fails with
`-FS_EBUSY` (16). Nothing is undone: the storage is marked stopped, the
volume stays attached and the caller gets EBUSY.

## Required behaviour

1. `storage_sync()` MUST succeed when the namespace is attached and the only
   pins on a volume are process cwd references (the supervisor's or any live
   process's) and the namespace's structural nodes (`/`, `/mnt`, `/dev`,
   `/dev/null`, `/dev/console`). A cwd pin does not block a volume detach,
   exactly as the legacy per-drive cwd does not (`vfs_detach` resets it to
   the root instead of refusing). Define in the contract what a process
   whose cwd was on the detached volume observes afterwards (the storage is
   stopped at that point, so the simplest consistent rule is acceptable:
   path resolution through that cwd fails `ENOENT`).
2. `storage_sync()` MUST still return `-EBUSY` while an open description on
   the volume exists (a native fd on a file or directory, or a legacy VFS
   handle), and a refused shutdown MUST leave a defined state: either the
   storage is not stopped and a later `storage_sync()` after the close
   succeeds, or it is stopped with every volume still consistent. Choose,
   document, test.
3. The hooks stay weak so the storage host test without the namespace keeps
   its behaviour; the F1 probes and the `fd-table` probe are not modified.

## Host tests

- A host test that links `posixpath.c` and `fdtable.c` with the storage
  harness (`FS_HOST`): attach the namespace, pin a cwd on the root as
  `files_bootstrap` does, run the production `probe_fat_write`,
  `probe_cache` and `probe_bootlog` paths or an equivalent direct
  `storage_sync()` → `0`; with an open fd on a file of the volume →
  `-EBUSY` and the defined state; after closing it → `0` (if rule 2 chooses
  the recoverable variant).
- Existing `test_storage`, `files_test`, `desktop_test` results unchanged.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f1:fat-write`, `f1:cache` and
`f1:bootlog` PASS on `qemu-t23`, the `f1-fat32` suite passes, and the
`fd-table` durable checker reports `flush_result=0`. Reply with: files, the
rule chosen for each point above, the contract amendment text, test output.
