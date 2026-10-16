# f1-79 — split the VFS namespace lock from per-volume locks; media queries never wait for a volume's I/O

- **Phase:** F1 (storage). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f1-79` from main (after `2fef3ffc`).
- **Authority:** `src/kernel/fs/vfs.h`, `vfs.c`, `mount.c`, `fat.c`, `fs_ro.c`, `cache.c`, **and every native caller that holds `vfs.lock`**: `src/kernel/proc/posixpath.c`, `fdtable.c`, `syscalls_file.c`, `src/kernel/drivers/media_dev.c`, the DOS VM disk gate, optical/media paths, `storage_remove_removable`; host tests `tests/host/fs/`, `tests/host/proc/`, `tests/host/usb/`.

## Evidence (T23, image 43d72a5c, `legacy/local/physical/43d72a5c/BOOT2.LOG` black box and BOOT1/BOOT2 live logs, read-only)

- Owner: with the stick mounted, Files shows its content but the whole system lags; clicking inside the stick turns Files into "Not responding".
- `[desktop] stall stage=shell … wait=media-read … folder_wait=directory-compare` every 500 ms up to `no_frame_ms=3266` while `[cpu] top=files 22–44 %`: the desktop frame thread sits in `media_read` (26 `MEDIA_GET_VOLUME` devctls, `media_dev.c:67 fs_lock_take(&s->vfs.lock)`) behind Files' syscalls on E:, each of which holds the single `vfs.lock` across a stick command (f1-78 part 1 made the lock FIFO and reduced commands 13×, so each wait is now one command; 26 waits still add up to seconds).
- f1-78 part 1 (merged `8fa13e7f`) left the lock split to this directive because native callers hold `vfs.lock` across filesystem operations.

## Required

1. A short namespace lock for node/description/table bookkeeping, attach/detach and path identity; one lock per drive letter for FAT metadata, chains and I/O; lock order namespace → volume → cache → device, documented in `vfs.h`, enforced by the pthread checker ranks (`fs_lock_classify`) on every entry point, native ones included. Native identities (shared offsets, share claims, cwd pins, observers) keep their exclusion rules; surprise removal and the DOS VM disk gate keep their guarantees.
2. `media_dev.c`: `MEDIA_GET_STATE`, `MEDIA_GET_VOLUME` and `MEDIA_GET_BLOCKERS` read a published snapshot (seqlock or the short namespace lock) and never wait on a volume lock or device I/O; `MEDIA_SAFE_REMOVE` keeps its exclusion.
3. The two-volume latency test from f1-78 item 4(a): a 25 ms-per-read fake volume scanned by thread A (listing + stat + open/read of 64 files) while thread B lists and reads a fast volume and thread C issues `MEDIA_GET_VOLUME`: every call of B and C completes in under 50 ms during A's scan.
4. `[storage] vfs_wait drive=%c ms=%u holder=%c reason=%s` (verbose class) when a namespace or volume wait exceeds 100 ms.
5. Host coverage: all of `tests/host/fs` (power-cut, write-in-place, log recovery, namespace, throughput, cache), `tests/host/proc`, `tests/host/usb/run.sh`, `storage_run.sh`, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`.
6. Report `build/codex/f1-79-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. FAT durability rules unchanged; f1-74 transfer limits unchanged; the f1-78 cache transport design stays.
