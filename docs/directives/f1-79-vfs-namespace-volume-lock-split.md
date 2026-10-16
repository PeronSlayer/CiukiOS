# f1-79 — split the VFS namespace lock from per-volume locks (follow-up of f1-78)

- **Phase:** F1 (storage). **Model:** gpt-6.1-sol / xhigh. **Status:** planned (not launched); write the full directive before launching.
- **Authority:** `src/kernel/fs/vfs.h`, `vfs.c`, `mount.c`, `fat.c`, `fs_ro.c`, **and the native callers that hold `vfs.lock` across filesystem operations**: `src/kernel/proc/posixpath.c`, `fdtable.c`, `syscalls_file.c`, the DOS VM disk gate, optical/media paths, `storage_remove_removable`; host tests `tests/host/fs/`, `tests/host/proc/`, `tests/host/usb/`.

## Why

f1-78 part 1 (merged 2026-10-17, `8fa13e7f`) made the filesystem locks FIFO, moved block-cache transport out of the cache lock and added cluster/FAT read-ahead (13× fewer USB commands on the listing workload). The single `vfs.lock` is still held across device I/O by every entry point, legacy and native, so a slow volume still delays every other volume's syscalls by one command at a time. The desktop no longer blocks its frame thread on storage (f2-79), so the user-visible freeze is gone; this directive removes the structural serialization.

## Required (outline)

1. A short namespace lock for node/description/table bookkeeping, attach/detach and path identity; one lock per drive letter for FAT metadata, chains and I/O; lock order namespace → volume → cache → device, documented in `vfs.h` and enforced by the pthread checker (`fs_lock_classify` ranks) on every entry point, native ones included.
2. Native identities (shared offsets, share claims, cwd pins, observers) keep their exclusion rules under the new locks; surprise removal and the DOS VM disk gate keep their guarantees.
3. The two-volume latency test from f1-78 item 4(a): a 25 ms-per-read fake volume scanned by thread A while thread B lists a fast volume; every call of B completes in under 50 ms.
4. `[storage] vfs_wait drive=%c ms=%u holder=%c reason=%s` (verbose class) when a namespace or volume wait exceeds 100 ms.
