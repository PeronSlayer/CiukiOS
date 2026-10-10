# Directive f4-03: NTFS read-only library (`ntfs_ro`) with host fixtures

- **Step:** F4. **Contracts:** `f4-audio-media-contract.md` ("NTFS
  read-only mount: driver and license", acceptance row T0 optical/NTFS;
  lead decision 7: an independent GPL-2.0-only driver informed by the
  versioned NTFS3 layout sources, no code import), `vfs-storage-contract.md`
  (no private block cache; read workspaces bounded and charged),
  `foundations-transition.md` (license rules).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f4-ntfs-ro`, from `main` at or after `67f6397`.
  Files: new `src/kernel/fs/ntfs_ro.c` and `src/kernel/include/ciuki/ntfs_ro.h`
  as a freestanding library (block reads through a caller-supplied
  callback, no VFS integration: that follows the `fs_ro.h` interface of
  f4-02 in a later directive), new `scripts/test/ntfs_fixtures.py`,
  host tests under `tests/host/`, `docs/design/f4-acceptance.md` (your
  section `## f4-03`; create the file if absent), `docs/build-and-run.md`
  (fixture tooling), and the license inventory entry for the reference
  sources (no NTFS3 code is copied).

## Scope

1. **Library.** Mount from a block-read callback (`lba`, count, buffer,
   bounded by the volume), parse the boot sector (bytes per sector,
   sectors per cluster, MFT/MFTMirr LCN, record sizes), `$MFT` mapping
   with its own runlist, FILE/INDX update-sequence fixups, MFT reference
   sequence-number validation, resident and non-resident unnamed `$DATA`
   including fragmented runlists (signed relative LCNs, VCN coverage,
   sparse and uninitialized tail zeroed without exposing slack), bounded
   attribute-list extensions, `$I30` index root/allocation/bitmap
   directory traversal, `$UpCase` collation and UTF-16 → UTF-8 names,
   DOS short names as aliases of the same node; all walks with cycle,
   iteration and memory limits; `EOPNOTSUPP` for compressed, encrypted,
   sparse-only-where-unsupported, reparse points and alternate streams;
   refusal with distinct reasons for dirty/recovery-required and
   hibernated volumes; zero block writes by construction (the callback
   is read-only).
2. **Fixtures.** `scripts/test/ntfs_fixtures.py` builds deterministic
   NTFS 3.1 images without privileges using the host tools present
   (`mkntfs -F -q` with fixed serial and timestamps where the tool allows,
   `ntfscp` to populate, `ntfsls`/`ntfscat` for independent listings and
   hashes): small clean volume; large directory (thousands of entries,
   `$I30` allocation and bitmap); fragmented data (interleaved writes and
   deletions); long and non-ASCII names; a compressed volume (`mkntfs -C`)
   for the `EOPNOTSUPP` path; a >4 GiB sparse extent via `ntfstruncate`
   for offsets beyond 32 bits and the initialized-length tail; patched
   variants for the dirty flag, a hibernation marker, corrupted fixups,
   runlists and index records. Each fixture records tool versions and
   SHA-256 of the image and of every file.
3. **Host tests:** the NTFS part of the T0 row: every fixture read through
   the library matches the independent hashes and listings; corrupted
   variants fail with the documented errors and never read outside the
   volume; refusal reasons; image hash unchanged after every test.

## Out of scope

VFS mount integration, drive letters, DOS bridge visibility, QEMU cases
(a later directive attaches the library to `fs_ro.h` and adds `f4:ntfs-ro`).

## Acceptance by the lead

Host tests; the library compiles into the kernel build without being
reachable yet (or as a host-only object if the kernel build would carry
dead code: decide and record); `f2-all` unaffected.
