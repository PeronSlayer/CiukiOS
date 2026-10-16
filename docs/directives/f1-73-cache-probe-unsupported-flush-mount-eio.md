# f1-73 — `cache` probe: the unsupported-flush view fails to mount with EIO since the mount budget

- **Phase:** F1 (filesystem). **Model:** gpt-6.1-sol / high. **Worktree:** `wt/f1-73` from main `032a9e12`+.
- **Authority:** `src/kernel/fs/fat.c` (mount/enable_write), `src/kernel/fs/cache.c`, `src/kernel/probes/fat_probes.c` (the `cache` probe only), `tests/host/fs/`.

## Evidence (image a34118c1, case `cache` of `tests/suites/f1-fat32.json`, run under `build/test-runs/zz-fs/`)

```
probe=cache … case=error layer=block_driver fault=flush delayed=-5 flush=-5 unmount=-5 mounted=1
probe=cache … case=ata_error fault=write delayed=-5 flush=-5 unmount=-5 mounted=1 quarantined=1
probe=cache … case=ata_quarantine owner=synthetic_ata generation=245 further_commands=0 real_commands=2 next=-200
probe=cache … case=unsupported_flush mode=rw upgrade=-5 reasons=0
probe=cache … group=metadata subcase=complete owner=vfs generation=1 errors=1 gate=failed timing_domain=icount
probe=cache … event=END status=FAIL error=-5
```

The probe (`fat_probes.c` around line 560) mounts a second, read-only forwarding view of the boot fixture with `noflush.flush = 0` and expects `fat_enable_write` to return `-FS_EROFS` with `readonly` set (`enable_write`: `!blkdev_durable` → `FAT_RO_DURABILITY`). It got `-5` with `mode=rw`, which means `fat_mount` of the view, or the cache error lookup for the copied `struct blkdev`, now fails with EIO before the durability rule runs. The case passed in suite context on 2026-10-11 (`docs/validation/2026-10-11-integrated-image-f0-regression.md`). Candidates since then: f1-70 (mount budget: ownership bitmap in the cache workspace, bounded scans), f0-25 (repair, write gate), f0-32 (in-place writes, tail trim).

## Required

1. Reproduce on the host with the production `fat_mount`/`cache` and a copied `struct blkdev` without `flush` (as the probe does), on top of a live mount of the same device, and name the exact `-FS_EIO` return (file:line) and its trigger (cache workspace busy, device table slot, ownership scan of a second view, write-gate state inherited through the copy, or a real I/O refusal).
2. Decide with the contract (`docs/design/vfs-storage-contract.md`): a second read-only view of a mounted device must mount, and the durability rule (`-FS_EROFS` + `readonly`) must still be the answer for a device without flush. Fix the kernel side, not the expectation, unless the probe's second view is itself invalid under the f1-70 budget; in that case say so and adjust the probe with the lead's wording recorded in the validation note.
3. Host coverage for the second view and the durability refusal; run `scripts/test/host_fs_tests.sh` and `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, then `python3 scripts/build_kernel.py`.
4. Report in `build/codex/f1-73-report.md` (cause, fix file:line, verbatim results). The lead reruns the `cache` case on QEMU.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
