# F1-73 cache probe and unsupported flush

Research checked before implementation on 2026-10-16:

- [Linux explicit volatile write-back cache control](https://www.kernel.org/doc/html/latest/block/writeback_cache_control.html): a durability operation must reach stable media through a qualified flush or a device without volatile caching. Ciuki's existing `blkdev_durable()` and `fat_enable_write()` enforce this distinction; a readable device lacking flush remains read-only.
- [Linux mutex semantics](https://docs.kernel.org/locking/mutex-design.html): mutual exclusion serializes participating callers. It cannot suppress an intentional synchronous write issued by the lock owner. Adding a namespace lock would not isolate the recorder call described below.

Read-only evidence: the newest run directory when inspected was
`/home/peronslayer/Desktop/CiukiOS/build/test-runs/zz-fs/f2e350aa/`.
Its `serial.log` ends the first synthetic ATA fault with
`case=ata_quarantine ... further_commands=0 real_commands=2 next=-200`,
then immediately reports `case=unsupported_flush mode=rw upgrade=-5 reasons=0`.
There is no second synthetic ATA fault record.
The run's `result.json` records image SHA-256
`a34118c1bea05399737e2461aa146aee2b66273c5130231b935b18358f3eac0d`
before and after the run.

The exact EIO is the final conditional return in `cache_ata_faults()`
(`src/kernel/probes/fat_probes.c:507` before this change):
`ata_command_count() != real`. `probe_cache()` then skips `fat_mount()`
because `e` is already nonzero, and labels that earlier error as the
unsupported-flush upgrade result. The zero-initialized static volume has
never been mounted, explaining its apparent `rw` mode and zero reasons.

The two real commands come from `storage_cache_fault()` calling
`fat_unmount()` on its synthetic volume. `fat_commit()` observes sticky
cache EIO, `write_error()` calls `fat_revoke_write()`, and the latter
checkpoints the global raw blackbox recorder on the real boot disk. A raw
record write and qualified flush increment the native ATA command count.
This is a synchronous diagnostic side effect, not a cache workspace,
device-table, ownership-scan, copied write-gate or mount-read failure.
The preceding block-fixture errors have the same synthetic-unmount side
effect, although they are outside that ATA counter interval.
`git blame -L 29,39 src/kernel/fs/fat.c` attributes the checkpoint call to
`bc0fc826` (2026-10-15); the read-gate revocation itself predates it.

The host regression binds the production raw recorder to an independent
sparse sector transport, then calls the production `storage_cache_fault()`.
Before the fix, its first write-fault subcase reports exactly
`F1-73 synthetic fault=write raw_writes=1 raw_flushes=1` and fails the
zero-recorder-command assertion. This independently reproduces the guest's
two native commands without running QEMU or changing the mount expectation.

Host reproduction on unchanged production FAT/cache confirms that copied
`storage_volume.io` devices with flush removed mount successfully over live
FAT12/16/32 mounts in both requested read-only and writable sessions. Each
upgrade returns `-FS_EROFS`, retains `FAT_RO_DURABILITY` and read-only mode,
and issues no writes or flushes to the backing volume. FAT16/32 dirty
sessions also exercise allocation failure and the reserved workspace.
The second read-only view is valid under F1-70. No FAT mount or cache
semantics need changing, and no suite expectation is weakened.

Implementation decision: model the synthetic unmounts as already revoked
by their known delayed write/flush error, with the write-error reason
present. The production unmount still checks sticky cache EIO and refuses
to detach, but does not treat the artificial volume as a new real-volume
revocation requiring a raw checkpoint. Preserve the production first-error
checkpoint for actual volumes and the existing global zero-native-command
assertion. Return immediately after an earlier cache subcase fails, before
emitting an unsupported-flush result for a skipped mount; include the mount
result in the successful-path record for diagnosis.

Qualification results are recorded in `build/codex/f1-73-report.md`.
Both required host suites and `python3 scripts/build_kernel.py` completed
with exit 0, sequentially under `nice -n 19`; the kernel FPU/SIMD audit
passed. The host script's optional prebuilt-DOSBOOT replay remained skipped.
After the fix, the same recorder regression reports zero raw writes and
flushes for both injected fault types; its positive control confirms that
revoking a live mounted volume still checkpoints the recorder. Genuine
second-view read failures return EIO after exactly one issued read without
detaching the original mount.
Guest evidence remains the lead's responsibility on the integrated image.
