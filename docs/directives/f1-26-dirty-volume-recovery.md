# Directive f1-26: automatic recovery of a dirty boot volume at mount

- **Step:** F1. **Owner decision (2026-10-11):** when a FAT volume is
  found dirty (clean-shutdown bit cleared by an unclean power-off) and the
  mount scan finds no corruption, the kernel clears the dirty bit and
  mounts the volume read/write; this is the default policy at least for
  the hardware diagnostic phase (diary entry 2026-10-11-07).
- **Contracts to amend:** `f1-acceptance.md` (`mount-crash` row and the
  dirty/error-flag sentence), `vfs-storage-contract.md` (mount scan,
  read-only reasons, durable clearing of the dirty bit), the Microsoft FAT
  specification (FAT16 cluster-1 entry bit 15 / FAT32 bit 27 =
  `ClnShutBitMask`; bit 14 / bit 26 = `HrdErrBitMask`).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f1-dirty-recovery`, from `main` at or after `438ff23`.
  Files: `src/kernel/fs/fat.c` and the FAT headers, `src/kernel/fs/mount.c`,
  `src/kernel/core/init.c` only for the activation record reason,
  `src/kernel/probes/fat_probes.c` (mount record fields), `tests/host/fs/*`,
  `tests/suites/f1-fat32.json` (`mount-dirty`, `mount-crash-reboot` and the
  crash checker expectations), `scripts/test/run.py` and
  `scripts/test/mount_fixtures.py` only for those predicates/checkers,
  `tests/host/test_runner.py`, `tests/host/test_mount_fixtures.py`, the two
  contract documents.

## Observed on the ThinkPad T23 (image `b9c052cd…`, serial capture `a1b2c3d4`, 2026-10-11)

After a desktop session powered off without unmount, the next boot
mounts C: read-only: `[storage] kind=2 disk=0 part=1 mode=ro … reason=mount`
and `[storage] disk_log=unavailable error=-30`. The kernel sets the dirty
bit when it enables writes and clears it only at a clean `storage_sync`;
nothing ever clears it after a power loss, so one unclean shutdown makes
every later boot read-only (no boot log, write probes refused) until the
image is rewritten.

## Required behaviour

1. At mount, when the clean-shutdown bit is cleared and the hardware-error
   bit is set, keep today's behaviour (read-only, reason `error-flag`).
2. When only the clean-shutdown bit is cleared: run the existing mount
   scan; if it reports `lost=0`, no corruption and no FAT-copy divergence,
   clear the bit durably in every FAT copy (write + barrier, same order
   rules as the clean unmount), mount read/write subject to the usual read
   gate, record `reason=dirty_recovered` in the activation entry and the
   mount record (`recovered=1`), and log one `[storage]` line. If the scan
   finds anything, keep read-only with the corruption reason as today.
3. Durability: the clearing write MUST be a barrier-ordered sector write
   that cannot leave the two FAT copies divergent (write copy 1, barrier,
   copy 2, barrier; a cut between them must be classified by the existing
   divergence scan on the next boot and then recovered or refused per the
   rules).
4. The hardware-error bit is never cleared automatically.

## Suites and contract

- `mount-dirty` (fixture: dirty bit only, consistent volume) now expects
  recovery: `recovered=1`, mode `rw` after the gate, the bit clear on the
  exported volume (`fsck.fat -n` no longer reports the dirty bit), zero
  refusals.
- `mount-error-flag`, `mount-fat-divergence`, `mount-chain-corruption`,
  `mount-torn-sector`, `mount-bad-bpb`: unchanged (still refused or
  read-only).
- `mount-crash-reboot`: boot 2 recovers the volume when the scan is clean
  (`lost=0 scan_corrupt=0`) and the orphan long-name entry is still the
  declared outcome; the predicates and the checker expectations change
  accordingly (no `EROFS` refusal when recovered; the dirty diagnostic is
  gone on the export taken after boot 2); keep "no cross-links" strict.
- Contract paragraphs in both documents state the rule and the owner's
  decision date.

## Host tests

Storage harness: dirty-only fixture recovers; dirty + lost cluster,
dirty + divergence, error-flag stay read-only; the clearing write order
under a simulated cut between the two FAT copies; predicate tests from
the probe's real record formatting.

## Acceptance by the lead

Host tests; kernel build; on QEMU the seven `mount-*` cases and
`bootlog`/`bootlog-read-only` PASS with the new predicates, then the full
`f2-all` batch; on the T23 a boot after an unclean power-off mounts C:
read/write with `reason=dirty_recovered`. Reply with: files, the exact
bit operations and write order, the contract text, test output.
