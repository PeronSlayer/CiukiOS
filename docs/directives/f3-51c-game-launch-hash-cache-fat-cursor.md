# f3-51c — fast game launch: base digest cached per base identity, cluster cursor per open file in the FAT read path

- **Phase:** F3 (DOS VMs) and F1 (storage). **Model:** Claude Opus agent (Codex out of credit until 2026-10-25). **Worktree:** `wt/f3-51c` from main.
- **Context:** `docs/directives/f3-51-*.md` amendment 2 and dev diary 2026-10-20-04: after the fix `85b9024a` a DOS game window still hashes its whole base (27,336,704 bytes for `C:\GAMES\CIUKIGAM.IMG`) inside CREATE at every launch; on QEMU TCG DOOM leaves "Preparing..." only after 60–90 s and the desktop marks the window "Not responding". Each sequential 64 KiB read also walks the FAT cluster chain from the file's first cluster (`fat_read` → `chain_at`, `src/kernel/fs/fat.c`), so sequential reads of large files are quadratic in the chain length.
- **Authority:** `src/kernel/vm/dosvm_disk.c` (digest cache), `src/kernel/fs/fat.c` and the VFS open-file state it needs (cluster cursor), `tests/host/` (dosvm, dos launch, fs suites), `docs/design/` notes for both changes.
- **Not yours:** the f3-51 item 8 guarantee itself (overlay names `G<g>K<k><sha>V<gen>.COW`, the `FREEDOS.SHA` check, the `game_launch` record) stays as specified.

## Required

1. **Digest cache.** Keep the SHA-256 of a game base per base identity — volume, first cluster, size, modification time and the file's write generation (whatever the VFS exposes that changes on every write; add a generation if none exists) — for the session, bounded (for example 16 entries, LRU). A launch whose base identity matches reuses the digest; any write, truncate, rename-over or volume remount invalidates it. The first launch after boot still hashes once. Record `cached=1|0` in the `game_launch` record.
2. **FAT cluster cursor.** Each open file keeps the last (file offset → cluster) pair it resolved; `fat_read`/`fat_write` continue the chain from it when the new offset is at or after the cursor, and fall back to the chain start otherwise. The cursor is invalidated by truncate, chain relocation and any FAT mutation of that file's chain (follow the f1-84 publication groups and the f1-83 request model; Windows 98 VFAT keeps the current cluster per open file — cite the Resource Kit or DDK source you use).
3. **Evidence.** Host tests: digest reuse and every invalidation path; sequential read of a 27 MB file on a FAT32 fixture with a FAT-lookup counter showing linear behaviour (state the counts before and after); the fs suite (`scripts/test/host_fs_tests.sh`), the kernel suite (`scripts/test/host_kernel_tests.sh`), the DOS suites (`python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py'`, `tests/host/dos/run_file_api.sh`, `run_hook_cpu.sh`), `make CAP= apps`. Report `build/codex/f3-51c-report.md` with a host timing of the launch hash before/after and what the lead must check on QEMU (DOOM launch time, second launch, launch after a write to the games disk).

## Constraints

rg and sed, never semble; no QEMU, no systemd-run, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; never disable sanitizer leak detection where the repository script does not; no new `panic()` paths; English.
