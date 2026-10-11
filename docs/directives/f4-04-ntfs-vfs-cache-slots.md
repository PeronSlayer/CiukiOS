# Directive f4-04: NTFS read-only mounts through `fs_ro` and optical cache slots on demand

- **Step:** F4. **Contracts:** `f4-audio-media-contract.md` ("NTFS read-only
  mount", acceptance rows T0 optical/NTFS and T3 `f4-ntfs-ro`),
  `vfs-storage-contract.md` (drivers keep no private block cache; the host
  VFS owns cache and visibility; partition types; `EROFS`), directives
  f4-02 (the `fs_ro` interface, ISO 9660 as its first driver, the mixed
  2048/512 cache slot layout recorded as a decision) and f4-03 (the
  `ntfs_ro` library and its fixtures).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f4-ntfs-vfs`, from `main` at or after `9c38d57`.
  Files: `src/kernel/fs/ntfs_ro.c` (adapter to `fs_ro.h`: probe by
  partition type `07h` and the NTFS boot signature, mount through the
  common cache, node/lookup/readdir/read operations, generation), `fs_ro.c`
  and `mount.c` (driver registration and letter assignment), `cache.c`
  (point 2), `src/kernel/probes/f4_probes_media.c` (`f4:ntfs-ro` probe
  added beside `optical`, registered in selector/loader/model tables as
  f4-02 did), `scripts/test/run.py` and `scripts/test/ntfs_fixtures.py`
  (fixture disks for QEMU from the f4-03 generator, attached as an IDE
  disk with an MBR partition of type `07h`), `tests/suites/f4-media.json`
  additions, host tests, `docs/design/f4-acceptance.md` (`## f4-04`),
  `docs/build-and-run.md`.

## Scope

1. **NTFS mounts.** Partitions of type `07h` on qualified ATA disks whose
   boot sector passes the f4-03 validation mount read-only through `fs_ro`
   with the f4-03 refusal reasons (dirty, recovery-required, hibernated,
   malformed) recorded as mount diagnostics; names through `$UpCase`
   collation and the VFS UTF-8 policy; DOS short names as aliases of the
   same node; `EOPNOTSUPP` for compressed/encrypted/reparse/alternate
   streams; sparse holes read as zeros (f4-03 decision reconciled with the
   contract: ordinary sparse files are readable, compressed ones are not;
   record it in the acceptance document); every mutation `EROFS`; zero
   block writes verified by instrumentation; media generation rules as
   for optical mounts (a disk cannot change under the system, but the
   mount still carries a generation for unmount/remount).
2. **Cache slots on demand.** Replace the fixed one-2048-plus-three-512
   slot layout per cache page with page layouts chosen per device sector
   size: a cache page serves either eight 512-byte slots or two 2048-byte
   slots, allocated when a device of that sector size is attached, within
   the unchanged budget; a system without an optical device keeps the
   full FAT slot count of F1; the F1 `cache` probe records and predicates
   stay valid (the probe's expected hit/miss arithmetic must be rechecked
   and the acceptance document updated if it changes); a failed dirty FAT
   block still must not monopolise a minimum cache (keep f4-02's rule).
3. **Probe `f4:ntfs-ro`.** With a declared NTFS fixture disk: mount by
   letter, list and hash the fixture's files against the generator's
   independent manifest (`ntfscat` hashes), attempt all mutations
   (`EROFS`), read a FAT peer before and after, unmount and remount,
   verify zero writes and the fixture image hash unchanged by the runner;
   refusal fixtures (dirty, hibernated, compressed file) report the
   expected reasons; cases on `qemu-t23` and `qemu-desktop-2002`; without
   a fixture `not_run reason=fixtures_absent`.
4. **Host tests:** the adapter against the f4-03 fixtures through the VFS
   harness used by f4-02 (native file reads, directory traversal, aliases,
   stale handles), the cache layout per sector size (slot counts, budget,
   mixed devices, failed dirty block rule), zero-write instrumentation.

## Acceptance by the lead

Host tests; kernel and image build; QEMU `f4-media` NTFS cases PASS with
hashes equal to the manifest and zero writes; F1 `cache` and `f1-fat32`
cases unchanged; `f2-all` unaffected.
