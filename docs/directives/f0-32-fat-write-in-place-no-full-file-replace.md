# f0-32 — Every file write replaces the whole file (quadratic): in-place and append writes for the FAT driver

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-32` from `main`. Build the kernel and the DOS image before reporting.

## Evidence (found by f2-73 on image a395ffa8)

`src/kernel/proc/syscalls_file.c:119` limits a write to one page under the VFS lock; `src/kernel/proc/fdtable.c:197` calls `fat_write`; `src/kernel/fs/fat.c:552` delegates to the full-file copy-on-write `replace` (line 509): appending a 3 MiB wallpaper cache in 768 pages copies the growing file 768 times, about 1.2 GB of payload writes plus metadata and barriers, and the shell's directory poll stalls on the shared filesystem. The same path serves settings saves, the registry, game overlays and every application file, on a 1 GHz machine with an ATA PIO disk. The boot log already has its own reserved-chain writer (f0-22).

## Task

1. Replace `replace`-per-write with in-place writes: overwriting an existing extent writes the affected sectors (read-modify-write for partial sectors) and appending allocates clusters at the chain's end, extending the chain and publishing the size once per write call; the full-file copy remains only for truncation-with-rewrite semantics the API requires (say which, if any). Durability ordering: data before chain, chain before directory size, with the existing barriers; crash cuts at every step leave either the old or the new length with no lost clusters (the f0-25 repair covers the rest).
2. The page-sized write limit and the VFS lock scope: allow multi-page writes in one call (bounded, e.g. 64 KiB) and release the lock between runs so the desktop's directory poll does not wait for a 3 MiB write.
3. Host fixtures: the wallpaper-cache append (3 MiB in 4 KiB and 64 KiB writes) measured in sectors written (must be about 6144 plus metadata, not millions), settings rewrite, registry update, overlay write, and the f0-25 crash-cut harness run again on the new path.

## Acceptance

Filesystem and kernel host suites green; QEMU `f0-smoke`, `f2-desktop` cases, `f3-vm` green; on the T23 the wallpaper cache write completes in under a second and the settings save is instant.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`; references: the FAT specification and FreeDOS/FatFs write paths, cited in the report `build/codex/f0-32-report.md`.
